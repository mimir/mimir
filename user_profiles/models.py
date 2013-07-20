from django.db import models
from django.contrib.auth.models import User
from lessons.models import Lesson, Question

class UserProfile(models.Model):
    user = models.OneToOneField(User, related_name='profile')
    #TODO implement picture stuff properly
    gravatar_hash = models.CharField(max_length = 32)
    website = models.URLField(blank = True)
    about = models.TextField(blank = True)
    created = models.DateField(auto_now_add = True) #Creation date set on adding
    logged_in = models.DateField(auto_now_add = True) # TODO make update on all logins
    def __unicode__(self):
        return self.screen_name

class UserTakesLesson(models.Model):
    user = models.ForeignKey(User)
    lesson = models.ForeignKey(Lesson)
    date = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    RATING_CHOICES = (
        (1, 1),
        (2, 2),
        (3, 3),
        (4, 4),
        (5, 5),
        (6, 6),
        (7, 7),
        (8, 8),
        (9, 9),
        (10, 10),
    )
    rating = models.PositiveSmallIntegerField(choices = RATING_CHOICES, blank=True)
    comment = models.CharField(max_length = 250, blank=True)
    def __unicode__(self):
        return self.user.username + " " + self.lesson.name

class UserAnswersQuestion(models.Model):
    user = models.ForeignKey(User)
    question = models.ForeignKey(Question)
    question_seed = models.DecimalField(max_digits = 21, decimal_places = 20)
    date = models.DateTimeField(auto_now_add = True) #Creation date set on adding
    correct = models.BooleanField()
    answer = models.CharField(max_length = 250) 
