from django.db import models
from django.contrib.auth.models import User
from lessons.models import Lesson, Question

class UserProfile(models.Model):
    user = models.OneToOneField(User, related_name='profile')
    #TODO implement picture stuff properly
    gravatar_hash = models.CharField(max_length = 32)
    website = models.URLField(blank = True)
    about = models.TextField(blank = True)
    def __unicode__(self):
        return self.user.username

class UserTakesLesson(models.Model):
    user = models.ForeignKey(User)
    lesson = models.ForeignKey(Lesson)
    date = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    RATING_CHOICES = [(i+1,i+1) for i in range(10)]
    rating = models.PositiveSmallIntegerField(choices = RATING_CHOICES, blank=True)
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
