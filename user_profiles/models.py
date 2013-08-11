from django.db import models
from django.contrib.auth.models import User
from django.db.models.signals import post_save
from django.dispatch import receiver
from lessons.models import Lesson, Question
import hashlib

class UserProfile(models.Model):
    user = models.OneToOneField(User, related_name='profile')
    #TODO implement picture stuff properly
    gravatar_hash = models.CharField(max_length = 32)
    website = models.URLField(blank = True)
    caption = models.CharField(max_length = 60, blank = True)
    about = models.TextField(blank = True)
    reputation = models.IntegerField(default = 0)
    def __unicode__(self):
        return self.user.username

class UserTakesLesson(models.Model):
    user = models.ForeignKey(User)
    lesson = models.ForeignKey(Lesson)
    date = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    RATING_CHOICES = [(i,i) for i in range(10)]
    rating = models.PositiveSmallIntegerField(choices = RATING_CHOICES, default = 0)
    comment = models.CharField(max_length = 250, blank=True)
    class Meta:
        ordering = ['date']
    def __unicode__(self):
        return self.user.username + " " + self.lesson.name

class UserAnswersQuestion(models.Model):
    user = models.ForeignKey(User)
    question = models.ForeignKey(Question)
    question_seed = models.DecimalField(max_digits = 21, decimal_places = 20)
    date = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    correct = models.BooleanField()
    answer = models.CharField(max_length = 250)


@receiver(post_save, sender=User)
def create_user_profile(sender, instance, created, **kwargs):
    if created:
        UserProfile.objects.create(user=instance, gravatar_hash = hashlib.md5(instance.email.strip().lower()).hexdigest())

#post_save.connect(create_user_profile, sender=User)
